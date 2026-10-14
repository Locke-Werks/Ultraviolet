#include "uvcore/Dither.h"

#include <cmath>
#include <cstdint>
#include <random>

namespace uv {

void ditherTo(AudioBuffer& buffer, int targetBits) {
    // Float / high-precision output keeps full precision: leave the samples
    // exactly as they are so the path is bit-exact.
    if (targetBits >= 32) {
        return;
    }

    // Quantization step. A targetBits grid places integers at value * 2^(bits-1),
    // so one LSB spans 2^-(bits-1).
    const double lsb = std::ldexp(1.0, -(targetBits - 1));
    const double invLsb = std::ldexp(1.0, targetBits - 1);

    // Signed integer range of the grid. Clamp keeps results inside
    // [-1, 1 - 1LSB]; the positive top rail is one LSB below unity because the
    // most positive representable code is (2^(bits-1) - 1).
    const double maxCode = invLsb - 1.0; // 1 - 1LSB in grid units
    const double minCode = -invLsb;      // -1 in grid units

    for (int ch = 0; ch < buffer.channelCount(); ++ch) {
        // Deterministic per-channel seeding so tests are reproducible. A
        // distinct offset per channel keeps channels from sharing a sequence.
        std::mt19937 rng(0x9E3779B9u + static_cast<std::uint32_t>(ch));
        std::uniform_real_distribution<double> uni(-0.5, 0.5);

        // First-order error-feedback state, in grid (LSB) units.
        double errorFeedback = 0.0;

        for (Sample& s : buffer.channels[ch]) {
            // TPDF dither: sum of two independent uniforms, one LSB peak-to-peak
            // each, in grid units.
            const double tpdf = uni(rng) + uni(rng);

            // Work in grid units so rounding is a plain nearest-integer op.
            const double shaped = s * invLsb + errorFeedback;
            const double dithered = shaped + tpdf;

            double code = std::nearbyint(dithered);
            if (code > maxCode) code = maxCode;
            if (code < minCode) code = minCode;

            // Feed the quantization error (pre-clamp target vs. chosen code)
            // back into the next sample. Using the dithered value as the
            // reference shapes total error, not just the rounding residue.
            errorFeedback = dithered - code;

            s = code * lsb;
        }
    }
}

} // namespace uv
