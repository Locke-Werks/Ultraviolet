#include "uvcore/Resampler.h"

#include "CDSPResampler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace uv {

namespace {

std::vector<double> runOneshot(const std::vector<double>& in, double srcRate, double dstRate,
                               int outFrames, FilterPhase phase) {
    std::vector<double> inCopy(in);
    std::vector<double> out(static_cast<std::size_t>(outFrames), 0.0);
    const r8b::EDSPFilterPhaseResponse rp =
        phase == FilterPhase::Minimum ? r8b::fprMinPhase : r8b::fprLinearPhase;
    // Same transition band and attenuation as CDSPResampler24's defaults.
    r8b::CDSPResampler resamp(srcRate, dstRate, (std::max)(1, static_cast<int>(inCopy.size())),
                              2.0, 180.15, rp);
    resamp.oneshot(inCopy.data(), static_cast<int>(inCopy.size()), out.data(), outFrames);
    return out;
}

// oneshot() back-dates the latency it reports, which for a minimum-phase
// filter is not where the impulse response peaks. Measure the residual offset
// once per rate pair from an actual impulse and cache it.
long long minPhaseOffset(double srcRate, double dstRate) {
    static std::mutex m;
    static std::map<std::pair<double, double>, long long> cache;
    std::lock_guard<std::mutex> lock(m);
    const auto key = std::make_pair(srcRate, dstRate);
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    const int inLen = 8192;
    const int at = 4096;
    std::vector<double> imp(static_cast<std::size_t>(inLen), 0.0);
    imp[static_cast<std::size_t>(at)] = 1.0;
    const int outLen = static_cast<int>(std::lround(inLen * dstRate / srcRate));
    const auto lin = runOneshot(imp, srcRate, dstRate, outLen, FilterPhase::Linear);
    const auto mp = runOneshot(imp, srcRate, dstRate, outLen, FilterPhase::Minimum);
    auto peak = [](const std::vector<double>& v) {
        return static_cast<long long>(
            (std::max_element)(v.begin(), v.end(),
                             [](double a, double b) { return std::abs(a) < std::abs(b); }) -
            v.begin());
    };
    const long long off = peak(mp) - peak(lin);
    cache[key] = off;
    return off;
}

} // namespace

AudioBuffer resample(const AudioBuffer& input, double targetRate, FilterPhase phase) {
    // Equal rates: nothing to do, hand back a copy unchanged.
    if (input.sampleRate == targetRate) {
        return input;
    }

    AudioBuffer out;
    out.sampleRate = targetRate;
    out.sourceBitDepth = input.sourceBitDepth;
    out.channels.resize(input.channels.size());

    const std::size_t inFrames = input.frames();
    if (inFrames == 0) {
        return out;
    }

    // Output length is the rate-scaled input length, rounded. oneshot() emits
    // exactly this many samples, back-dating its internal filter latency so
    // sample n of the output lines up with sample n of the input.
    const int outFrames = static_cast<int>(std::lround(
        static_cast<double>(inFrames) * targetRate / input.sampleRate));

    if (phase == FilterPhase::Linear) {
        // One stateful resampler per channel: the filter carries state across
        // the stream, so channels cannot share an instance.
        for (std::size_t ch = 0; ch < input.channels.size(); ++ch) {
            std::vector<double> inCopy(input.channels[ch].begin(), input.channels[ch].end());
            std::vector<double> outCh(static_cast<std::size_t>(outFrames), 0.0);
            r8b::CDSPResampler24 resamp(input.sampleRate, targetRate, static_cast<int>(inFrames));
            resamp.oneshot(inCopy.data(), static_cast<int>(inCopy.size()), outCh.data(), outFrames);
            out.channels[ch] = std::move(outCh);
        }
        return out;
    }

    // Minimum phase: render with extra tail, then drop the measured offset so
    // the peak of every transient stays where the linear path puts it.
    const long long off = minPhaseOffset(input.sampleRate, targetRate);
    const long long extra = (std::max)(0LL, off);
    for (std::size_t ch = 0; ch < input.channels.size(); ++ch) {
        std::vector<double> in(input.channels[ch].begin(), input.channels[ch].end());
        const auto full = runOneshot(in, input.sampleRate, targetRate,
                                     outFrames + static_cast<int>(extra), FilterPhase::Minimum);
        std::vector<double> outCh(static_cast<std::size_t>(outFrames), 0.0);
        for (long long i = 0; i < outFrames; ++i) {
            const long long j = i + off;
            if (j >= 0 && j < static_cast<long long>(full.size())) outCh[static_cast<std::size_t>(i)] = full[static_cast<std::size_t>(j)];
        }
        out.channels[ch] = std::move(outCh);
    }
    return out;
}

} // namespace uv
