#pragma once

#include "uvcore/Types.h"

namespace uv {

// High-quality, arbitrary-ratio resampler (r8brain-free-src backed).
// Handles integer ratios (48k->192k) and rational ones (44.1k->192k).
AudioBuffer resample(const AudioBuffer& input, double targetRate);

} // namespace uv
