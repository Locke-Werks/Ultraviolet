#pragma once

#include <cstddef>
#include <vector>

namespace uv {

// Core sample type. Offline, quality-first: process in double throughout.
using Sample = double;

// Planar (de-interleaved) audio: one vector of samples per channel.
struct AudioBuffer {
    std::vector<std::vector<Sample>> channels;
    double sampleRate = 0.0;
    int sourceBitDepth = 0; // informational: bit depth of the decoded source

    int channelCount() const { return static_cast<int>(channels.size()); }

    std::size_t frames() const {
        return channels.empty() ? 0u : channels.front().size();
    }

    bool empty() const { return frames() == 0; }
};

} // namespace uv
