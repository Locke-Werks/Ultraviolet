#pragma once

#include "uvcore/Types.h"

namespace uv {

// Quantize a buffer to targetBits with TPDF dither + noise shaping, in place.
// Samples remain stored as double but are quantized to the target grid.
// A no-op for float output (which keeps full precision).
void ditherTo(AudioBuffer& buffer, int targetBits);

} // namespace uv
