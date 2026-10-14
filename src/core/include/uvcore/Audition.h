#pragma once

#include "uvcore/Types.h"

namespace uv {

// Tools for listening to the synthesized extension on its own. Most of it
// sits above 20 kHz, so these isolate it and move it into hearing range.

// The content the pipeline added: processed minus the plain resample of the
// source to processed's rate. Exact because the extension adds nothing below
// its crossover, so the original cancels. Output has processed's rate and
// length.
AudioBuffer addedComponent(const AudioBuffer& processed, const AudioBuffer& source);

// Divides every frequency by 2^octaves while keeping duration and timing, so
// it plays in sync with the original. octaves in [1, 3]; 0 returns a copy.
AudioBuffer pitchShiftDown(const AudioBuffer& buffer, int octaves);

// Divides every frequency by factor by slowing playback (bat-detector time
// expansion). Exact: samples are unchanged, only the rate drops, so duration
// grows by factor.
AudioBuffer timeExpand(const AudioBuffer& buffer, double factor);

// Scales the buffer so its peak sits at targetDbfs. Returns the gain applied
// in dB, or 0 for a silent buffer (left unchanged).
double normalizePeak(AudioBuffer& buffer, double targetDbfs = -6.0);

} // namespace uv
