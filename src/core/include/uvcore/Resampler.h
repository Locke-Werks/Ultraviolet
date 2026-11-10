#pragma once

#include "uvcore/Types.h"

namespace uv {

// High-quality, arbitrary-ratio resampler (r8brain-free-src backed).
// Handles integer ratios (48k->192k) and rational ones (44.1k->192k).
// With FilterPhase::Minimum the output is shifted so an impulse peaks where
// the linear-phase path would put it, keeping comparisons sample-aligned.
AudioBuffer resample(const AudioBuffer& input, double targetRate,
                     FilterPhase phase = FilterPhase::Linear);

} // namespace uv
