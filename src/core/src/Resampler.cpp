#include "uvcore/Resampler.h"

#include "CDSPResampler.h"

#include <cmath>
#include <vector>

namespace uv {

AudioBuffer resample(const AudioBuffer& input, double targetRate) {
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

    // Output length is the rate-scaled input length, rounded. oneshot() below
    // emits exactly this many samples, back-dating its internal filter latency
    // so sample n of the output lines up with sample n of the input.
    const int outFrames = static_cast<int>(std::lround(
        static_cast<double>(inFrames) * targetRate / input.sampleRate));

    const int maxInLen = static_cast<int>(inFrames);

    // One stateful resampler per channel: the filter carries state across the
    // stream, so channels cannot share an instance.
    for (std::size_t ch = 0; ch < input.channels.size(); ++ch) {
        // oneshot() takes a non-const input pointer, so copy the channel.
        std::vector<double> inCopy(input.channels[ch].begin(),
                                   input.channels[ch].end());

        std::vector<double> outCh(static_cast<std::size_t>(outFrames), 0.0);

        r8b::CDSPResampler24 resamp(input.sampleRate, targetRate, maxInLen);
        resamp.oneshot(inCopy.data(), static_cast<int>(inCopy.size()),
                       outCh.data(), outFrames);

        out.channels[ch] = std::move(outCh);
    }

    return out;
}

} // namespace uv
