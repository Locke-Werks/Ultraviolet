#pragma once

#include "uvcore/Types.h"

namespace uv {

// Gain of the air shelf at frequency hz for a given top gain. Flat below
// 4 kHz, rising smoothly on a log-frequency axis to the full gain at 20 kHz,
// then held. The shape was fitted to the tilt measured on a PGGB plus mastering
// VST reference, which climbs about 6.5 dB from 4 to 20 kHz.
double airShelfGainDb(double hz, double topDb);

// Applies the shelf in place, linear phase, so transients keep their shape.
// topDb <= 0 leaves the buffer untouched.
void applyAirShelf(AudioBuffer& buffer, double topDb);

} // namespace uv
